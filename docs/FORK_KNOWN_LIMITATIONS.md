# LibreCAD-mcp: known limitations

What this fork does not do yet, or does only in part. Each item says whether upstream LibreCAD
behaves the same way.

## Reading AutoCAD 2018-2026 DWG (AC1032)

Every AutoCAD from 2018 to 2026 saves the same DWG format, AC1032 ("2018" in the version lists).
LibreCAD 2.2.1.x refuses it ("unsupported DXF/DWG file version"); upstream master and this fork read
it. On top of upstream master this fork:

- keeps the content of paper-space layouts out of model space, and writes it back on save
  (DXF R2000 or later, DWG 2000 or later);
- drops dictionary entries whose item handle is null instead of failing the whole dictionary
  (AutoCAD writes such entries, for example `ACAD_PARALLEL_BACKGROUND`);
- reads the CLASSES section CRC of R2007+ files at the right place (the spurious CRC warning on
  every 2007-2026 DWG is gone);
- decodes the CELLSTYLEMAP objects of R13-R2004 files;
- ships the `LINE` and `ANSI37` hatch patterns.

Limits that remain:

- **Layouts are kept, not shown.** LibreCAD has no paper-space or layout view. What a drawing
  holds on its layouts is kept in memory and written back on save, but you cannot see or edit it.
  A drawing made on its layouts alone opens with its active layout shown in model space, as
  LibreCAD always did.
- **Layout viewports are not read.** They are left out of every save, and the save report says
  how many.
- **DXF R12 and R14 hold the active layout only.** Saving to them leaves out the other layouts;
  the report says so, and Save refuses to overwrite a file with less than it holds (Save As offers
  DXF 2018).
- **Saving some AutoCAD drawings as DWG fails** (upstream does the same): the native DWG writer
  refuses objects it cannot reproduce exactly, such as some TABLESTYLE owner graphs and
  DataStorage. LibreCAD then says the export failed and writes nothing. Save as DXF 2018 instead;
  every AutoCAD and LibreCAD version reads it.
- **Dictionaries in LibreCAD-written DWG 2007-2018 are not readable by LibreDWG** (upstream does
  the same). LibreDWG-based tools may then lose the layouts of such a file. DWG 2000/2004 output
  and DXF output are not affected.
- **OLE frames are not written to DWG**; the save report lists them.
- **Hatch pattern density can differ from AutoCAD's** (upstream does the same): LibreCAD's
  pattern library is not drawn at AutoCAD's pattern scale. This fork adds `LINE` and `ANSI37`,
  which AutoCAD drawings use often, at the density of LibreCAD's `ANSI31`. A hatch whose pattern
  LibreCAD does not have is still not drawn.

## Remote API and `librecad-mcp`

- The Remote API is off by default and listens on `127.0.0.1` only. Anyone who can read the
  discovery file (your user account) or the token can drive LibreCAD while the API is on.
- The token stays the same until you press **Regenerate** in Preferences; it is not rotated per
  run.
- While you have a dialog open in LibreCAD, every request answers `409 gui_modal_open`; the client
  goes on once you close it.
- See [the Remote API README](remote-api/README.md) and [the MCP server README](../mcp/README.md)
  for what each tool accepts.
