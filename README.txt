LiDE50 Scan 1.3 - portable scanning for the Canon CanoScan LiDE 50
=====================================================================

For 64-bit Windows 10 and 11. One file, no installer: LiDE50Scan.exe.
It supports only the Canon CanoScan LiDE 50 (USB ID 04A9:2213).


HOW IT WORKS
------------
Canon's driver (lide50vst7133a) is a 32-bit Windows XP/Vista driver and
can't be loaded on 64-bit Windows, so it couldn't be adapted. Instead this app
has its own scanner driver built in, the same approach VueScan uses. It talks
to the scanner's Genesys GL841 chip directly over USB, using the open-source
LiDE 50 support from the SANE project.

Windows only lets an app use a USB device after a driver has been assigned
to it. The first time you plug in the scanner, the app asks once for
administrator permission and assigns Windows' own built-in "WinUSB" driver
to the scanner. No third-party driver is installed, and you never need to do
this again on that PC. VueScan has the same one-time step.


FIRST RUN
---------
1. Plug in the LiDE 50. It is powered by USB, so there is no power cable.
   Unlock the scan head if your model has a lock switch.
2. Start LiDE50Scan.exe. You can run it from any folder or a USB stick.
3. Click "Enable scanner access (one-time)" and approve the Windows prompt.
4. The panel shows "Canon CanoScan LiDE 50 - Ready".


THE WINDOW
----------
- Left panel: scanner status, scan settings, adjustments and saving.
  Scroll it with the mouse wheel.
- Top toolbar: Preview and Scan, which become a progress bar and a Cancel
  button while scanning. On the right are Show glass, Fit to window,
  light/dark mode and the More menu (save all, clear all, scanner access,
  log files, background motion, About).
- Middle: the scanner glass with your preview and the selected area, the
  scan as it comes in, or the selected finished scan.
- Right panel: finished scans. Click one to view and adjust it.

The app follows Windows' light or dark setting until you press the
sun/moon button, which switches modes and remembers your choice. The
slowly drifting background pauses when the window isn't active, and turns
off when Windows animations are off. You can also switch it off in the
More menu.
Double-click any slider to reset it.


SCANNING
--------
- Preview (F5) does a quick low-resolution pass of the whole glass.
- Drag on the preview to choose the scan area. You can also pick a size
  (A4, Letter, photo sizes), or click "Auto-detect document".
- Choose Color, Grayscale or Black & white, and a resolution from 75 to 2400 dpi.
  300 dpi is good for documents, 600 dpi for photos.
- Scan (F6) runs the scan. You can also press the buttons on the scanner.
  The middle of the window shows the scan coming in line by line. Esc cancels.
- Finished scans appear in the list on the right. Click one to edit it.

Adjustments update the image live and can be changed later. They include
brightness, contrast, saturation, gamma, sharpness, noise/descreen,
black & white threshold, auto levels, invert, rotate and mirror. "Apply to
all scans" copies one scan's settings to the others.

The first scan at each resolution takes a few extra seconds while the
scanner calibrates. The calibration is cached after that.
After a scan the image appears straight away, while the scan head travels
back to the start of the glass (up to about 7 seconds after a full-length
scan). The toolbar shows "Returning scan head" during that time. You can
already look at, adjust and save the scan, and choose the next area, but
Preview and Scan wait until the head is back.


SAVING
------
File types: PDF, Word (.docx), PNG, JPEG, TIFF and BMP.
- "Save this scan" saves the selected scan.
- "Save all" puts every scan into one multi-page PDF, Word or TIFF file. For
  PNG, JPEG or BMP it saves numbered files (name_001.png, name_002.png, ...).
Use Move up / Move down to change the page order before saving.
The resolution (dpi) is stored in every file, so prints come out the right size.


PORTABLE DATA
-------------
Settings and the calibration cache are kept in "LiDE50Scan-data" next to the
exe. If that folder can't be written to, they go in
%LOCALAPPDATA%\LiDE50Scan instead. Nothing is written to the registry
except the one-time WinUSB assignment.


ACTIVITY LOG
------------
Everything the program does is written to LiDE50Scan.log, right next to
LiDE50Scan.exe. The file is created the first time the program starts, and
each run is added to the end. It records:
  - startup details (Windows version, folders, USB status)
  - your clicks and setting changes
  - every step of each preview or scan: lamp warm-up, calibration
    results, progress, transfer sizes and timing
  - all warnings and errors, and if the program ever crashes, the error
    type and the exact place in the code where it happened
Lines are written as they happen, so the log is complete even after a crash.
A preview plus a scan adds roughly 300 KB (it used to be several MB).
"..." > "Open the log file" shows it. When the log passes 20 MB it is renamed
to LiDE50Scan.old.log and a new one starts.
If something goes wrong, start the program with "LiDE50Scan.exe --log" and
repeat what you did. That also records every register access and every USB
transfer, which is what's needed to track down a scanner fault (it adds a
few MB per scan).
If the program folder is read-only (for example inside Program Files), the
log goes to %LOCALAPPDATA%\LiDE50Scan instead.


TROUBLESHOOTING
---------------
* "The scan head did not move or the sensor saw no light": turn the scanner
  over. If there is a lock switch, slide it to UNLOCK. Then close the lid,
  unplug the scanner, plug it back in and try again.
* "Automatic setup did not succeed": use Scanner > Manual setup instructions.
  In short: open Device Manager, right-click the CanoScan device, choose
  Update driver > Browse my computer > Let me pick > Universal Serial Bus
  devices > WinUsb Device.
* USB errors: plug the scanner straight into the PC rather than through a
  hub, then unplug and replug it.
* Streaky or dark scans: make sure the scan head is unlocked, close the lid,
  and delete LiDE50Scan-data\.sane\canon-lide-50.cal to force a new calibration.
* To undo the setup, use Scanner > Remove scanner access.
* Any problem: send LiDE50Scan.log (Help > Show the log file in its folder).
* Demo without a scanner: run "LiDE50Scan.exe --simulate".


LICENSES
--------
LiDE50 Scan is free software under the GNU General Public License v2 or
later (see COPYING). It contains:
  - the SANE genesys backend (GL841 / LiDE 50), GPL-2.0-or-later
  - libusb 1.0.27, LGPL-2.1-or-later
  - miniz (MIT) and stb_image_write (public domain)
The complete source code is in LiDE50Scan-source.zip and at
https://github.com/ifloofie/LiDE50-scanner (credits with links to each
project are on that page).
Canon and CanoScan are trademarks of Canon Inc. This app is not affiliated
with or endorsed by Canon.
