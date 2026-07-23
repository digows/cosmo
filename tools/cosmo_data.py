"""
Reading and writing the game's data files.

Enough of the formats to build a new episode: group files, and the map format
that carries a level's layout, its actors and its choice of backdrop and music.

The formats are not documented by Apogee. Everything here was derived from the
game's own loading code in `vendor/cosmore` -- `GroupEntryFp()` for the group
files, `LoadMapData()` and `InitializeLevel()` for the maps -- and checked by
round-tripping all eleven maps of episode 1.
"""

import struct
from dataclasses import dataclass, field

# A group file opens with a header of 20-byte records: a 12-byte name, then a
# 32-bit offset and length. The game reads only the first 960 bytes of it, so
# no more than 48 entries are reachable however large the file is. Data begins
# at 4000, which is where the original files put it.
GROUP_ENTRY_SIZE = 20
GROUP_READABLE_ENTRIES = 48
GROUP_DATA_START = 4000

# Tile values below this are air, or a platform direction command. From
# TILE_STRIPED_PLATFORM in graphics.h.
TILE_AIR_LIMIT = 0x0050

# At and above this a tile is masked, and the backdrop shows through its
# transparent parts. From TILE_MASKED_0.
TILE_MASKED_BASE = 0x3E80

# Map actor types below 32 are "special" -- the player's start position, moving
# platforms, fountains and lights -- rather than creatures. From actor.h.
SPA_PLAYER_START = 0


def read_group(path):
    """Return {name: bytes} for every entry the game can reach."""
    with open(path, "rb") as fp:
        raw = fp.read()

    entries = {}
    for i in range(0, GROUP_READABLE_ENTRIES * GROUP_ENTRY_SIZE, GROUP_ENTRY_SIZE):
        name = raw[i:i + 12].split(b"\0")[0].decode("latin-1")
        if not name:
            break
        offset, length = struct.unpack("<II", raw[i + 12:i + 20])
        entries[name.upper()] = raw[offset:offset + length]
    return entries


def write_group(path, entries):
    """Write {name: bytes} as a group file."""
    if len(entries) > GROUP_READABLE_ENTRIES:
        raise ValueError(
            f"{len(entries)} entries, but the game only searches the first "
            f"{GROUP_READABLE_ENTRIES}"
        )

    header = bytearray(GROUP_DATA_START)
    body = bytearray()

    for slot, (name, data) in enumerate(entries.items()):
        encoded = name.upper().encode("latin-1")
        if len(encoded) > 12:
            raise ValueError(f"entry name too long: {name}")

        offset = GROUP_DATA_START + len(body)
        record = slot * GROUP_ENTRY_SIZE
        header[record:record + 12] = encoded.ljust(12, b"\0")
        header[record + 12:record + 20] = struct.pack("<II", offset, len(data))
        body += data

    with open(path, "wb") as fp:
        fp.write(bytes(header) + bytes(body))


@dataclass
class Map:
    """
    A level.

    `flags` packs the level's presentation, decoded in InitializeLevel():

        bits 0-4    backdrop, indexing backdropNames[]
        bit  5      rain
        bit  6      backdrop scrolls horizontally
        bit  7      backdrop scrolls vertically
        bits 8-10   palette animation
        bits 11-15  music, indexing musicNames[]

    `width` must be a power of two between 32 and 2048; the game derives a
    shift from it and will silently misbehave otherwise. Height follows from
    how many tiles are supplied.

    `actors` are (type, x, y) triples. Type 0 is the player's start.

    `tiles` is one word per cell, row-major.
    """

    width: int
    tiles: list = field(default_factory=list)
    actors: list = field(default_factory=list)
    backdrop: int = 0
    music: int = 0
    rain: bool = False
    scroll_h: bool = True
    scroll_v: bool = False
    palette_animation: int = 0

    @property
    def height(self):
        return len(self.tiles) // self.width

    @property
    def flags(self):
        return (
            (self.backdrop & 0x1F)
            | (0x20 if self.rain else 0)
            | (0x40 if self.scroll_h else 0)
            | (0x80 if self.scroll_v else 0)
            | ((self.palette_animation & 0x07) << 8)
            | ((self.music & 0x1F) << 11)
        )

    def to_bytes(self):
        if self.width not in (1 << n for n in range(5, 12)):
            raise ValueError(f"width {self.width} is not a power of two in 32..2048")

        flat = []
        for actor in self.actors:
            flat.extend(actor)

        return (
            struct.pack("<HHH", self.flags, self.width, len(flat))
            + struct.pack(f"<{len(flat)}H", *flat)
            + struct.pack(f"<{len(self.tiles)}H", *self.tiles)
        )

    @classmethod
    def from_bytes(cls, data):
        flags, width, actor_words = struct.unpack("<HHH", data[:6])
        actors_raw = struct.unpack(f"<{actor_words}H", data[6:6 + actor_words * 2])
        rest = data[6 + actor_words * 2:]
        tiles = struct.unpack(f"<{len(rest) // 2}H", rest[:len(rest) // 2 * 2])

        return cls(
            width=width,
            tiles=list(tiles),
            actors=[tuple(actors_raw[i:i + 3]) for i in range(0, actor_words, 3)],
            backdrop=flags & 0x1F,
            music=(flags >> 11) & 0x1F,
            rain=bool(flags & 0x20),
            scroll_h=bool(flags & 0x40),
            scroll_v=bool(flags & 0x80),
            palette_animation=(flags >> 8) & 0x07,
        )
