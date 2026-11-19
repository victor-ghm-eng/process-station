# Ignition configuration (live export, Ignition 8.3.10 Maker Edition)

Exported from the running gateway on 19 Nov 2026 (after the fixes of section 7.6 of the report). No credentials are included.

| Path | Content | How to import |
|---|---|---|
| `udt_AnalogSensor.json` | UDT definition `AnalogSensor` (Value, Setpoint, HighLimit, Units; HighTemperature alarm; history) | Tag Browser > UDT Definitions > right-click > Import Tags. Import this first. |
| `tags_ProcessStation.json` | Folder `ProcessStation`: `Status` (HR0-HR10), `Commands` (HR20-HR26) and UDT instance `TT101` | Tag Browser > `[default]` > right-click > Import Tags |
| `device_ProcessStation/` | Modbus TCP device `ProcessStation` (127.0.0.1:502, zero-based addressing) | Recreate in Gateway > Connections > Devices with the same name and settings (config.json shows them) |
| `ProcessStation_project_export.zip` | Same project as a gateway export zip | Gateway > Projects > Import, or `POST /data/api/v1/projects/import/ProcessStation` |
| `project_ProcessStation/` | Perspective project: views `Main`, `Popups/SensorDetail`, templates `StatusIndicator` and `CommandButton`, style classes `Indicator/*` and `Command/Button`, page config | Copy the folder into `data/projects/` (rename to `ProcessStation`) and restart, or rebuild from the view JSON |

Notes
- The tag `Status/Alarm` (HR7, latched Arduino alarm) is enabled and drives the LOCAL ALARM indicator; its own alarm is disabled so it does not duplicate the supervisory alarm on `TT101/Value`.
- UDT members Value, Setpoint and HighLimit are Float4; HighTemperature has deadband 0.5 (Absolute), a 2 s active delay and a 5 s clear delay (timeOffDelaySeconds).
- History is stored in the gateway's `Sample_SQLite_Database` historian; the Power Chart pen sources contain the gateway name (`ignition-desktop-3luhgqe`) and must be adjusted on another machine.
