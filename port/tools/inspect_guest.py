"""Read a live TTT2 guest range without writing or suspending the process.

Examples (use a /MAP from the exact running build):
  python port/tools/inspect_guest.py --pid 123 --map port/build/tekken_recomp.map 0x82000690 32
  python port/tools/inspect_guest.py --pid 123 --map port/build/tekken_recomp.map --compare-image re/decrypted/Default_patched_tu3.bin
  python port/tools/inspect_guest.py --pid 123 --map port/build/tekken_recomp.map --lookup 0x82738AD8 --lookup 0x82C3E1E8
  python port/tools/inspect_guest.py --pid 123 --map port/build/tekken_recomp.map --title-state
  python port/tools/inspect_guest.py --pid 123 --map port/build/tekken_recomp.map --menu-state

Comparison defaults to PPC_CODE_BASE/PPC_CODE_SIZE from re/ppc/ppc_config.h.
Reads are live, sequential observations, not an atomic snapshot. Only
PROCESS_VM_READ and PROCESS_QUERY_INFORMATION access rights are requested.
"""

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys
import time


ROOT = Path(__file__).resolve().parents[2]


def parse_map(text):
    preferred = re.search(r"Preferred load address is\s+([0-9A-Fa-f]+)", text)
    timestamp = re.search(r"Timestamp is\s+([0-9A-Fa-f]+)", text)
    if not preferred or not timestamp:
        raise ValueError("Map lacks preferred load address or timestamp")
    symbols = {}
    for name, value in re.findall(
        r"^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]+\s+(\S+)\s+([0-9A-Fa-f]{16})\s", text, re.M
    ):
        symbols[name] = int(value, 16) - int(preferred.group(1), 16)
    bases = [rva for name, rva in symbols.items() if name == "g_base" or name.startswith("?g_base@@")]
    if len(set(bases)) != 1:
        raise ValueError("Map must resolve exactly one g_base global")
    return {
        "name": text.split()[0],
        "preferred": int(preferred.group(1), 16),
        "timestamp": int(timestamp.group(1), 16),
        "g_base_rva": bases[0],
        "symbols": symbols,
    }


def read_config(path):
    text = path.read_text(encoding="utf-8-sig")
    result = {}
    for name in ("PPC_IMAGE_BASE", "PPC_IMAGE_SIZE", "PPC_CODE_BASE", "PPC_CODE_SIZE"):
        match = re.search(r"^#define\s+" + name + r"\s+(0x[0-9A-Fa-f]+)", text, re.M)
        if not match:
            raise ValueError(f"Missing {name} in {path}")
        result[name] = int(match.group(1), 16)
    return result


def compare_bytes(actual, expected, address, max_ranges):
    if len(actual) != len(expected):
        raise ValueError("Comparison lengths differ")
    total = 0
    range_count = 0
    ranges = []
    start = None
    for offset, (a, b) in enumerate(zip(actual, expected)):
        if a != b:
            total += 1
            if start is None:
                start = offset
                range_count += 1
        elif start is not None:
            if len(ranges) < max_ranges:
                ranges.append(diff_range(actual, expected, address, start, offset))
            start = None
    if start is not None and len(ranges) < max_ranges:
        ranges.append(diff_range(actual, expected, address, start, len(actual)))
    return {
        "different_bytes": total,
        "changed_ranges": range_count,
        "ranges": ranges,
        "ranges_omitted": range_count - len(ranges),
        "actual_sha256": hashlib.sha256(actual).hexdigest(),
        "expected_sha256": hashlib.sha256(expected).hexdigest(),
    }


def diff_range(actual, expected, address, start, end):
    preview_end = min(start + 32, end)
    return {
        "address": f"0x{address + start:08X}",
        "size": end - start,
        "actual_preview": actual[start:preview_end].hex(" "),
        "expected_preview": expected[start:preview_end].hex(" "),
    }


def read_title_state(read_guest, config):
    """Decode TU3 title state through its globals, using a read-only callback.

    825EEBD0/825EF548 install the mode object at mode_impl+4; 82625CE0
    installs TitleMode::Impl at object+4. Other modes must not follow this
    title-specific layout. Rereading the path detects some live transitions,
    but does not turn these sequential reads into an atomic snapshot.
    """
    def pointer(address, label):
        if not address or address & 3 or address > 0xFFFFF000:
            raise ValueError(f"Invalid {label} guest pointer: 0x{address:08X}")
        return address

    def read(address, size):
        data = read_guest(address, size)
        if len(data) != size:
            raise ValueError(f"Short title-state read at 0x{address:08X}")
        return data

    def u32(address):
        return struct.unpack(">I", read(address, 4))[0]

    def hex32(value):
        return f"0x{value:08X}"

    global_address = 0x82F30B94
    result = {"status": "uninitialized", "mode_global": hex32(global_address)}
    mode = u32(global_address)
    result["mode_impl"] = hex32(mode)
    if not mode:
        return result
    mode_header = read(pointer(mode, "mode implementation"), 16)
    mode_vtable, obj, requested_mode, current_mode = struct.unpack(">4I", mode_header)
    result.update(mode_vtable=hex32(mode_vtable), requested_mode=requested_mode, current_mode=current_mode)
    if mode_vtable != 0x820BDDF0:
        result["status"] = "unexpected_mode_vtable"
        return result
    if current_mode != 5 or requested_mode != 5:
        result["status"] = "not_title_mode"
        return result
    result["mode_object"] = hex32(obj)
    if not obj:
        return result
    object_vtable, title = struct.unpack(">2I", read(pointer(obj, "mode object"), 8))
    result.update(object_vtable=hex32(object_vtable), title_impl=hex32(title))
    if object_vtable != 0x820CCD5C:
        result["status"] = "unexpected_title_object_vtable"
        return result
    if not title:
        return result
    header = read(pointer(title, "title implementation"), 216)
    field = lambda offset: struct.unpack_from(">I", header, offset)[0]
    result["title_vtable"] = hex32(field(0))
    if field(0) != 0x820CCDA0:
        result["status"] = "unexpected_title_vtable"
        return result
    state, name_address = field(8), field(48)
    if state & 3 or not config["PPC_CODE_BASE"] <= state < config["PPC_CODE_BASE"] + config["PPC_CODE_SIZE"]:
        raise ValueError(f"Title state is outside aligned guest code: {hex32(state)}")
    if not config["PPC_IMAGE_BASE"] <= name_address < config["PPC_CODE_BASE"]:
        raise ValueError(f"Title state name is outside image data: {hex32(name_address)}")
    name_bytes = read(name_address, min(128, config["PPC_CODE_BASE"] - name_address))
    name, terminator, _ = name_bytes.partition(b"\0")
    if not terminator or not name.startswith(b"&TitleMode::Impl::state_"):
        raise ValueError(f"Unexpected TitleMode state name at {hex32(name_address)}")
    result.update(status="ok", state_function=hex32(state), state_name_address=hex32(name_address),
                  state_name=name.decode("ascii"), state_clock=struct.unpack_from(">Q", header, 40)[0],
                  duration_ticks_124=field(124),
                  flags={"132": header[132], "148": header[148], "149": header[149], "150": header[150]},
                  screen_wrapper=hex32(field(184)))
    if state == 0x82623D68:
        # movie_main uses this clock/completion predicate, not the bng_main active byte.
        # Its clrldi after addi deliberately keeps the subtraction unsigned32.
        threshold = (field(124) - 780) & 0xFFFFFFFF
        reached = result["state_clock"] >= threshold
        result["movie_transition"] = {"threshold_clock": threshold,
                                      "clock_reached_threshold": reached,
                                      "completion_override_148": header[148] != 0,
                                      "predicate_met": reached or header[148] != 0}
    movie = field(180)
    result["movie"] = {"object": hex32(movie), "completion_gate_in_current_state": state == 0x82624C20}
    # Later states retain a movie pointer but use their screen wrapper instead.
    # Do not dereference that potentially stale allocation outside bng_main.
    if state == 0x82624C20 and movie:
        inner = u32(pointer(movie, "movie object"))
        playback = u32(pointer(inner, "movie implementation") + 560) if inner else 0
        result["movie"].update(implementation=hex32(inner), playback_object=hex32(playback))
        result["movie"]["active_byte"] = read(pointer(playback, "movie playback") + 146, 1)[0] if playback else 0
    if (u32(global_address) != mode or read(mode, 16) != mode_header or
            read(obj, 8) != struct.pack(">2I", object_vtable, title) or
            u32(title + 8) != state or u32(title + 48) != name_address):
        result["status"] = "changed_during_read"
    return result


def read_menu_state(read_guest, config):
    """Read the verified TU3 MainMenu layout only while both mode IDs are 16.

    MainMenu's state member, clock and name share the base layout used by Title.
    The object/implementation vtables are installed by 825E5B38/825E5828;
    825E6500, 825E65B0 and 825E7B28 pair the three known main names with
    825E78C8. Other states may retain old pointers, which we do not follow.
    """
    def hex32(value):
        return f"0x{value:08X}"

    def pointer(address, label):
        if not address or address & 3 or address > 0xFFFFF000:
            raise ValueError(f"Invalid {label} guest pointer: {hex32(address)}")
        return address

    def read(address, size):
        data = read_guest(address, size)
        if len(data) != size:
            raise ValueError(f"Short menu-state read at {hex32(address)}")
        return data

    def u32(address):
        return struct.unpack(">I", read(address, 4))[0]

    global_address = 0x82F30B94
    result = {"status": "uninitialized", "mode_global": hex32(global_address),
              "mainmenu_main": False}
    mode = u32(global_address)
    result["mode_impl"] = hex32(mode)
    if not mode:
        return result
    mode_header = read(pointer(mode, "mode implementation"), 16)
    mode_vtable, obj, requested_mode, current_mode = struct.unpack(">4I", mode_header)
    result.update(mode_vtable=hex32(mode_vtable), requested_mode=requested_mode,
                  current_mode=current_mode, mode_object=hex32(obj))
    if mode_vtable != 0x820BDDF0:
        result["status"] = "unexpected_mode_vtable"
        return result
    if current_mode != 16 or requested_mode != 16:
        result["status"] = "not_mainmenu_mode"
        return result
    if not obj:
        return result
    object_header = read(pointer(obj, "mode object"), 8)
    object_vtable, menu = struct.unpack(">2I", object_header)
    result.update(object_vtable=hex32(object_vtable), menu_impl=hex32(menu))
    if object_vtable != 0x820BCE70:
        result["status"] = "unexpected_menu_object_vtable"
        return result
    if not menu:
        return result
    header = read(pointer(menu, "menu implementation"), 132)
    field = lambda offset: struct.unpack_from(">I", header, offset)[0]
    result["menu_vtable"] = hex32(field(0))
    if field(0) != 0x820BCE5C:
        result["status"] = "unexpected_menu_vtable"
        return result
    state, name_address = field(8), field(48)
    if state & 3 or not config["PPC_CODE_BASE"] <= state < config["PPC_CODE_BASE"] + config["PPC_CODE_SIZE"]:
        raise ValueError(f"Menu state is outside aligned guest code: {hex32(state)}")
    if not config["PPC_IMAGE_BASE"] <= name_address < config["PPC_CODE_BASE"]:
        raise ValueError(f"Menu state name is outside image data: {hex32(name_address)}")
    name_bytes = read(name_address, min(128, config["PPC_CODE_BASE"] - name_address))
    name, terminator, _ = name_bytes.partition(b"\0")
    if not terminator or not name.startswith(b"&MainMenuMode::Impl::state_"):
        raise ValueError(f"Unexpected MainMenuMode state name at {hex32(name_address)}")
    expected_main_name = b"&MainMenuMode::Impl::state_mainmenu_main"
    is_main = state == 0x825E78C8
    name_matches_main = (name == expected_main_name and
                         name_address in (0x820BC190, 0x820BC278, 0x820BC2A4))
    # A transition can publish the name before the member-function pointer.
    # Never certify mainmenu_main from a stale name or function alone.
    status = "ok" if is_main == name_matches_main else "state_name_mismatch"
    result.update(status=status, state_function=hex32(state),
                  state_name_address=hex32(name_address), state_name=name.decode("ascii"),
                  state_clock=struct.unpack_from(">Q", header, 40)[0],
                  flags={"120": header[120]}, screen_object=hex32(field(128)))
    if (u32(global_address) != mode or read(mode, 16) != mode_header or
            read(obj, 8) != object_header or u32(menu) != field(0) or
            u32(menu + 8) != state or u32(menu + 48) != name_address):
        result["status"] = "changed_during_read"
    result["mainmenu_main"] = result["status"] == "ok" and is_main
    return result


class ReadProcess:
    def __init__(self, pid):
        if os.name != "nt" or ctypes.sizeof(ctypes.c_void_p) != 8:
            raise ValueError("Live inspection requires 64-bit Windows Python")
        self.k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.k32.OpenProcess.restype = wintypes.HANDLE
        self.k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.k32.CloseHandle.restype = wintypes.BOOL
        self.k32.ReadProcessMemory.argtypes = [wintypes.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
        self.k32.ReadProcessMemory.restype = wintypes.BOOL
        self.handle = self.k32.OpenProcess(0x0010 | 0x0400, False, pid)
        if not self.handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            self.module = self.main_module(pid)
        except BaseException:
            self.close()
            raise

    def main_module(self, pid):
        class ModuleEntry(ctypes.Structure):
            _fields_ = [
                ("dwSize", wintypes.DWORD), ("th32ModuleID", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD), ("GlblcntUsage", wintypes.DWORD),
                ("ProccntUsage", wintypes.DWORD), ("modBaseAddr", ctypes.c_void_p),
                ("modBaseSize", wintypes.DWORD), ("hModule", wintypes.HMODULE),
                ("szModule", wintypes.WCHAR * 256), ("szExePath", wintypes.WCHAR * 260),
            ]

        self.k32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
        self.k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self.k32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ModuleEntry)]
        self.k32.Module32FirstW.restype = wintypes.BOOL
        snapshot = self.k32.CreateToolhelp32Snapshot(0x00000008 | 0x00000010, pid)
        if snapshot == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            entry = ModuleEntry()
            entry.dwSize = ctypes.sizeof(entry)
            if not self.k32.Module32FirstW(snapshot, ctypes.byref(entry)):
                raise ctypes.WinError(ctypes.get_last_error())
            return {"base": entry.modBaseAddr, "size": entry.modBaseSize, "path": entry.szExePath, "name": entry.szModule}
        finally:
            self.k32.CloseHandle(snapshot)

    def read(self, address, size):
        chunks = []
        for offset in range(0, size, 1024 * 1024):
            count = min(size - offset, 1024 * 1024)
            buffer = ctypes.create_string_buffer(count)
            received = ctypes.c_size_t()
            if not self.k32.ReadProcessMemory(self.handle, address + offset, buffer, count, ctypes.byref(received)):
                raise OSError(f"ReadProcessMemory at 0x{address + offset:X}: {ctypes.WinError(ctypes.get_last_error())}")
            if received.value != count:
                raise OSError(f"Short memory read at 0x{address + offset:X}: {received.value}/{count}")
            chunks.append(buffer.raw)
        return b"".join(chunks)

    def resolve_guest_base(self, linker):
        # An installed copy is renamed ("Tekken Tag Tournament 2.exe", test copies "TTT2 mNNN.exe"); the PE timestamp below
        # is the real check.
        stem = Path(self.module["name"]).stem.lower()
        if stem not in (linker["name"].lower(), "tekken tag tournament 2") and not stem.startswith(("ttt2 m", "ttt2 d")):
            raise ValueError("Map image name does not match process main module")
        header = self.read(self.module["base"], 64)
        if header[:2] != b"MZ":
            raise ValueError("Main module has no MZ header")
        pe = self.read(self.module["base"] + struct.unpack_from("<I", header, 0x3C)[0], 24)
        if pe[:4] != b"PE\0\0" or struct.unpack_from("<H", pe, 4)[0] != 0x8664:
            raise ValueError("Main module is not x64 PE")
        timestamp = struct.unpack_from("<I", pe, 8)[0]
        if timestamp != linker["timestamp"]:
            raise ValueError(f"Map/build mismatch: loaded timestamp 0x{timestamp:X}, map 0x{linker['timestamp']:X}")
        rva = linker["g_base_rva"]
        if not 0 <= rva <= self.module["size"] - 8:
            raise ValueError("g_base RVA falls outside main module")
        guest_base = struct.unpack("<Q", self.read(self.module["base"] + rva, 8))[0]
        if not guest_base or guest_base & 0xFFFF:
            raise ValueError(f"g_base is null or not allocation-aligned: 0x{guest_base:X}")
        return guest_base

    def close(self):
        if self.handle:
            self.k32.CloseHandle(self.handle)
            self.handle = None


def self_test():
    linker = parse_map("sample\n Timestamp is 1234\n Preferred load address is 0000000140000000\n 0003:00135d00 ?g_base@@3PEAEEA 000000014FAA5D00 object\n")
    assert linker["g_base_rva"] == 0xFAA5D00
    result = compare_bytes(b"aacdff", b"abcdef", 0x82000000, 1)
    assert result["different_bytes"] == 2 and result["changed_ranges"] == 2
    assert result["ranges"][0]["address"] == "0x82000001" and result["ranges_omitted"] == 1
    assert compare_bytes(b"aaa", b"bbb", 0, 2)["ranges"][0]["size"] == 3
    assert compare_bytes(b"abc", b"abc", 0, 2)["different_bytes"] == 0
    print("inspect_guest self-test: PASS (map RVA, sparse/end diff ranges, truncation, identical bytes)")
    config = {"PPC_IMAGE_BASE": 0x82000000, "PPC_CODE_BASE": 0x82250000, "PPC_CODE_SIZE": 0xB6848C}
    memory = {}

    def write(address, data):
        memory.update((address + offset, value) for offset, value in enumerate(data))

    def read(address, size):
        return bytes(memory.get(address + offset, 0) for offset in range(size))

    def word(address, value):
        write(address, struct.pack(">I", value))

    assert read_title_state(read, config)["status"] == "uninitialized"
    word(0x82F30B94, 0x10000)
    write(0x10000, struct.pack(">4I", 0x820BDDF0, 0x20000, 5, 5))
    write(0x20000, struct.pack(">2I", 0x820CCD5C, 0x30000))
    word(0x30000, 0x820CCDA0)
    word(0x30008, 0x82624C20)
    write(0x30028, struct.pack(">Q", 372))
    word(0x30030, 0x820CBF28)
    write(0x820CBF28, b"&TitleMode::Impl::state_bng_main\0")
    word(0x300B4, 0x40000)
    word(0x40000, 0x50000)
    word(0x50230, 0x60000)
    write(0x60092, b"\1")
    title = read_title_state(read, config)
    assert title["status"] == "ok" and title["title_impl"] == "0x00030000"
    assert title["state_clock"] == 372 and title["movie"]["active_byte"] == 1
    word(0x1000C, 7)
    assert read_title_state(read, config)["status"] == "not_title_mode"
    word(0x1000C, 5)
    word(0x30000, 0xDEADBEEF)
    assert read_title_state(read, config)["status"] == "unexpected_title_vtable"
    word(0x30000, 0x820CCDA0)
    word(0x40000, 0xFFFFFF00)
    try:
        read_title_state(read, config)
    except ValueError as error:
        assert "Invalid movie implementation" in str(error)
    else:
        raise AssertionError("Out-of-range movie pointer was accepted")
    word(0x40000, 0x50000)
    word(0x30008, 0x82624438)
    write(0x820CBF28, b"&TitleMode::Impl::state_havok_main\0")
    word(0x300B4, 0xDEADBEEF)  # A later state's stale movie pointer is not followed.
    assert read_title_state(read, config)["movie"]["completion_gate_in_current_state"] is False
    word(0x30008, 0x82623D68)
    write(0x820CBF28, b"&TitleMode::Impl::state_movie_main\0")
    word(0x3007C, 1000)
    title = read_title_state(read, config)
    assert title["duration_ticks_124"] == 1000 and title["movie_transition"]["threshold_clock"] == 220
    assert title["movie_transition"]["predicate_met"] is True
    word(0x3007C, 500)
    title = read_title_state(read, config)
    assert title["movie_transition"]["threshold_clock"] == 0xFFFFFEE8
    assert title["movie_transition"]["predicate_met"] is False
    write(0x30094, b"\1")
    title = read_title_state(read, config)
    assert title["flags"]["148"] == 1 and title["movie_transition"]["predicate_met"] is True
    assert title["movie_transition"]["completion_override_148"] is True
    reads = 0

    def changing_read(address, size):
        nonlocal reads
        if address == 0x82F30B94:
            reads += 1
            if reads == 2:
                return struct.pack(">I", 0)
        return read(address, size)

    assert read_title_state(changing_read, config)["status"] == "changed_during_read"
    print("inspect_guest title-state self-test: PASS (global chain, movie gates, unsigned threshold, inactive mode, vtables, invalid pointer, stale movie, live transition)")
    self_test_menu(config)
    if os.name == "nt" and ctypes.sizeof(ctypes.c_void_p) == 8:
        buffer = ctypes.create_string_buffer(b"guest-reader-smoke")
        proc = ReadProcess(os.getpid())
        try:
            assert proc.read(ctypes.addressof(buffer), len(buffer.raw)) == buffer.raw
            assert proc.read(proc.module["base"], 2) == b"MZ"
            print("inspect_guest Windows API self-test: PASS (module enumeration and read-only memory access)")
        finally:
            proc.close()


def self_test_menu(config):
    memory = {}

    def write(address, data):
        memory.update((address + offset, value) for offset, value in enumerate(data))

    def word(address, value):
        write(address, struct.pack(">I", value))

    def read(address, size):
        return bytes(memory.get(address + offset, 0) for offset in range(size))

    def fixture():
        memory.clear()
        word(0x82F30B94, 0x10000)
        write(0x10000, struct.pack(">4I", 0x820BDDF0, 0x20000, 16, 16))
        write(0x20000, struct.pack(">2I", 0x820BCE70, 0x30000))
        word(0x30000, 0x820BCE5C)
        word(0x30008, 0x825E78C8)
        write(0x30028, struct.pack(">Q", 123))
        word(0x30030, 0x820BC190)
        write(0x30078, b"\1")
        word(0x30080, 0xDEADBEEF)  # Observe but never dereference screen pointers.
        for address in (0x820BC190, 0x820BC278, 0x820BC2A4):
            write(address, b"&MainMenuMode::Impl::state_mainmenu_main\0")

    checks = 0

    def check(condition):
        nonlocal checks
        checks += 1
        assert condition

    check(read_menu_state(read, config)["status"] == "uninitialized")
    fixture()
    for name in (0x820BC190, 0x820BC278, 0x820BC2A4):
        word(0x30030, name)
        state = read_menu_state(read, config)
        check(state["status"] == "ok" and state["mainmenu_main"])
        check(state["state_clock"] == 123 and state["flags"]["120"] == 1)
        check(state["screen_object"] == "0xDEADBEEF")
    for offset in (8, 12):
        fixture()
        word(0x10000 + offset, 5)
        word(0x10004, 0xDEADBEEF)
        def no_object_read(address, size):
            assert address in (0x82F30B94, 0x10000)
            return read(address, size)
        state = read_menu_state(no_object_read, config)
        check(state["status"] == "not_mainmenu_mode" and not state["mainmenu_main"])
    for address, value, expected in (
        (0x10000, 0, "unexpected_mode_vtable"),
        (0x10004, 0, "uninitialized"),
        (0x20000, 0x820CCD5C, "unexpected_menu_object_vtable"),
        (0x20004, 0, "uninitialized"),
        (0x30000, 0x820CCDA0, "unexpected_menu_vtable"),
    ):
        fixture(); word(address, value)
        state = read_menu_state(read, config)
        check(state["status"] == expected and not state["mainmenu_main"])
    for address, value in (
        (0x82F30B94, 0x10001), (0x10004, 0xFFFFFF00),
        (0x20004, 0x30001), (0x30008, 0x825E78C9),
        (0x30008, 0), (0x30030, 0xDEADBEEF),
    ):
        fixture(); word(address, value)
        try:
            read_menu_state(read, config)
        except ValueError:
            check(True)
        else:
            raise AssertionError("Invalid menu pointer was accepted")
    fixture()
    word(0x30008, 0x825E7B28)
    check(read_menu_state(read, config)["status"] == "state_name_mismatch")
    word(0x30030, 0x820BC300)
    write(0x820BC300, b"&MainMenuMode::Impl::state_wait\0")
    check(read_menu_state(read, config)["status"] == "ok")
    check(not read_menu_state(read, config)["mainmenu_main"])
    word(0x30008, 0x825E78C8)
    check(read_menu_state(read, config)["status"] == "state_name_mismatch")
    for changed_address in (0x82F30B94, 0x10000, 0x20000, 0x30000, 0x30008, 0x30030):
        fixture()
        def changing_read(address, size):
            # Detail reads finish before the path is checked again.
            data = read(address, size)
            if address == 0x820BC190:
                word(changed_address, 0)
            return data
        state = read_menu_state(changing_read, config)
        check(state["status"] == "changed_during_read" and not state["mainmenu_main"])
    fixture()
    try:
        read_menu_state(lambda address, size: read(address, size)[:-1], config)
    except ValueError as error:
        check("Short menu-state read" in str(error))
    else:
        raise AssertionError("Short menu read was accepted")
    print(f"inspect_guest menu-state self-test: PASS ({checks} checks: mode/vtables, known names, stale state, invalid pointers, short reads, live transitions)")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    integer = lambda value: int(value, 0)
    parser.add_argument("address", nargs="?", type=integer, help="Guest address, decimal or 0x-prefixed")
    parser.add_argument("size", nargs="?", type=integer)
    parser.add_argument("--pid", type=int)
    parser.add_argument("--map", type=Path)
    parser.add_argument("--config", type=Path, default=ROOT / "re/ppc/ppc_config.h")
    parser.add_argument("--compare-image", type=Path)
    parser.add_argument("--lookup", type=integer, action="append", default=[], help="Inspect an indirect-call table slot for a guest code address (repeatable)")
    parser.add_argument("--title-state", action="store_true", help="Read TU3 named TitleMode state through the mode-manager global")
    parser.add_argument("--menu-state", action="store_true", help="Read TU3 named MainMenuMode state through the mode-manager global")
    parser.add_argument("--max-ranges", type=int, default=20)
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if args.pid is None or args.map is None:
        parser.error("--pid and --map are required")
    if (args.address is None) != (args.size is None):
        parser.error("address and size must be supplied together")
    if args.address is None and not args.compare_image and not args.lookup and not args.title_state and not args.menu_state:
        parser.error("Supply address/size, --compare-image, --lookup, --title-state, or --menu-state")
    if args.max_ranges < 0:
        parser.error("--max-ranges must be nonnegative")
    config = read_config(args.config)
    linker = parse_map(args.map.read_text(encoding="utf-8-sig", errors="replace"))
    address, size = args.address, args.size
    if address is None and args.compare_image:
        address, size = config["PPC_CODE_BASE"], config["PPC_CODE_SIZE"]
    if address is not None and (address < 0 or size <= 0 or address + size > 0x100000000 or size > 128 * 1024 * 1024):
        parser.error("Range must fit guest 4 GiB, with size 1..128 MiB")
    if address is not None and not args.compare_image and size > 65536:
        parser.error("Hex display is limited to 64 KiB; use --compare-image for larger ranges")
    proc = ReadProcess(args.pid)
    try:
        guest_base = proc.resolve_guest_base(linker)
        report = {"pid": args.pid, "module_path": proc.module["path"], "module_base": f"0x{proc.module['base']:X}", "map_timestamp": f"0x{linker['timestamp']:X}", "g_base_rva": f"0x{linker['g_base_rva']:X}", "guest_base": f"0x{guest_base:X}", "observation": "live sequential reads; process not suspended"}
        if args.title_state:
            report["title_state"] = read_title_state(lambda addr, count: proc.read(guest_base + addr, count), config)
        if args.menu_state:
            report["menu_state"] = read_menu_state(lambda addr, count: proc.read(guest_base + addr, count), config)
        if address is not None:
            started = time.perf_counter()
            actual = proc.read(guest_base + address, size)
            report.update(address=f"0x{address:08X}", size=size, read_ms=round((time.perf_counter() - started) * 1000, 3))
            if args.compare_image:
                image_offset = address - config["PPC_IMAGE_BASE"]
                if image_offset < 0:
                    raise ValueError("Range starts before image base")
                with args.compare_image.open("rb") as stream:
                    stream.seek(image_offset)
                    expected = stream.read(size)
                report["image"] = str(args.compare_image.resolve())
                report["comparison"] = compare_bytes(actual, expected, address, args.max_ranges)
            else:
                report["hex"] = [f"{address + offset:08X}  {actual[offset:offset+16].hex(' ')}" for offset in range(0, size, 16)]
        slots = []
        # Newer builds keep dispatch outside guest memory. Detect the loaded
        # build's layout from its map, never from today's generated header.
        host_table_rva = next((value for name, value in linker["symbols"].items() if name.startswith("?g_ppcFunctionTable@@")), None)
        host_table = None
        if args.lookup and host_table_rva is not None:
            host_table = struct.unpack("<Q", proc.read(proc.module["base"] + host_table_rva, 8))[0]
            if not host_table:
                raise ValueError("Loaded host dispatch table is not initialized")
        for target in args.lookup:
            if target & 3 or not config["PPC_CODE_BASE"] <= target < config["PPC_CODE_BASE"] + config["PPC_CODE_SIZE"]:
                raise ValueError(f"Lookup target 0x{target:X} is outside aligned guest code")
            displacement = (target - config["PPC_CODE_BASE"]) * 2
            slot = config["PPC_IMAGE_BASE"] + config["PPC_IMAGE_SIZE"] + displacement
            host_slot = guest_base + slot if host_table is None else host_table + displacement
            host = struct.unpack("<Q", proc.read(host_slot, 8))[0]
            symbol = f"__imp__sub_{target:08X}"
            expected_rva = linker["symbols"].get(symbol)
            if expected_rva is None:
                expected_rva = next((value for name, value in linker["symbols"].items() if name.startswith(f"?sub_{target:08X}@@")), None)
            expected = None if expected_rva is None else proc.module["base"] + expected_rva
            slots.append({"target": f"0x{target:08X}", "storage": "legacy guest memory" if host_table is None else "host memory", "guest_slot": f"0x{slot:08X}" if host_table is None else None, "host_slot": f"0x{host_slot:X}", "host": f"0x{host:016X}", "expected_host": None if expected is None else f"0x{expected:016X}", "matches_map": None if expected is None else host == expected})
        if slots:
            report["lookup_slots"] = slots
        if args.json:
            print(json.dumps(report, indent=2))
        else:
            for key, value in report.items():
                if key == "hex":
                    print("\n".join(value))
                elif isinstance(value, (dict, list)):
                    print(f"{key}: {json.dumps(value, indent=2)}")
                else:
                    print(f"{key}: {value}")
        return 0
    finally:
        proc.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(f"inspect_guest: {error}", file=sys.stderr)
        sys.exit(1)
