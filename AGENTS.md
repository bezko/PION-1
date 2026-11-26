# Memory

## Project Overview
See @README.md for project overview and @package.json for available npm/pnpm commands for this project.

## Code Style Guidelines
- Use descriptive variable names
- Follow existing patterns in the codebase
- Extract complex conditions into meaningful boolean variables

## Architecture Notes
Add important architectural decisions and patterns here.

## Common Workflows

Document frequently used workflows and commands here.

### PicoClaw Pico flashing

- Use the software-assisted BOOTSEL reboot instead of holding the physical BOOTSEL button:

  ```sh
  picotool reboot -u
  ```

- Confirm the Pico is in BOOTSEL mode and flash the firmware:

  ```sh
  picotool load -x build/PION1.uf2
  ```

- `picotool load -x` writes the downloaded program and performs the bootrom reboot to execute it.
- After flashing, the Pico automatically returns to application mode; verify with:

  ```sh
  picotool info -a
  ```

- For future flashing sessions, rerun `picotool reboot -u` before loading again. No physical button press is normally required.
- Hold BOOTSEL only as a manual fallback while connecting the board.
