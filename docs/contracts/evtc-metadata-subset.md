# EVTC metadata subset

This document specifies the only uncompressed EVTC fields GW2 Manny Uploader decodes. It is an
independent protocol description based on the public format documentation and writer published by
the arcdps author:

- <https://www.deltaconnected.com/arcdps/evtc/README.txt>
- <https://www.deltaconnected.com/arcdps/evtc/writeencounter.cpp>

The previous Rust uploader is a behavioral reference only. It delegates parsing to `revtc`; no parser
implementation from that project or dependency is copied or translated here.

## Supported payload

Version 1 accepts an uncompressed revision-1 EVTC payload in little-endian byte order:

| Section | Size | Required fields |
|---|---:|---|
| Header | 16 bytes | `EVTC` magic at 0, revision at 12, boss/species ID at 13 |
| Agent count | 4 bytes | Unsigned count |
| Agent table | 96 bytes each | Address at 0, elite marker at 12, combined name at 28 |
| Skill count | 4 bytes | Unsigned count |
| Skill table | 68 bytes each | Skipped by this decoder |
| Combat events | 64 bytes each | Source address at 8, state change at 56 |

The 64-byte agent name field contains character name, account name, and subgroup as consecutive
NUL-terminated UTF-8 strings. The account token is preserved exactly, including the conventional
leading `:`. The point-of-view event is state-change value 13 (`CBTS_POINTOFVIEW`); its source address
must identify a player agent with a non-empty, valid UTF-8 account segment.

## Validation and limits

The decoder:

- accepts revision 1 only;
- rejects bad magic, unsupported revisions, truncated sections, and partial combat events;
- caps agent, skill, and combat-event counts before iteration;
- performs checked offset arithmetic and never maps the bytes onto native C++ structs;
- checks cancellation before parsing and while scanning potentially large tables;
- reports a missing POV event, unknown POV agent, non-player POV agent, or empty account as
  `MissingPointOfView`; and
- reports malformed NUL-delimited or invalid UTF-8 account data as `MalformedLog`.

ZIP container validation and decompression are separate adapter responsibilities. The archive adapter
must enforce its own compressed/uncompressed size limits before calling this decoder.
