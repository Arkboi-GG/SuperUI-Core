- [![Linux Development Build](https://github.com/Yafrovon/SuperUI-Core/actions/workflows/linux-development-release.yaml/badge.svg?branch=main)](https://github.com/Yafrovon/SuperUI-Core/actions/workflows/linux-development-release.yaml) Current development build status
# SuperUI-Core

**SuperUI-Core** is a customized fork of [VMaNGOS](https://github.com/vmangos/core), designed to work alongside **MangosSuperUI**.

VMaNGOS provides the underlying World of Warcraft server emulator and progression framework. SuperUI-Core extends that foundation with the server-side systems, commands, data access, and gameplay integrations required by the MangosSuperUI platform.

The project is focused on making a persistent Vanilla world that can be observed, controlled, edited, automated, and expanded through a modern web interface.

## Relationship to VMaNGOS

SuperUI-Core remains fundamentally based on VMaNGOS and retains its core architecture, database structure, gameplay systems, and support for progressive Vanilla World of Warcraft clients.

The fork adds optional systems intended for MangosSuperUI rather than replacing the underlying VMaNGOS project.

Where practical, custom behavior is configurable so that standard VMaNGOS behavior can remain available.

## MangosSuperUI integration

SuperUI-Core provides the server-side support for features such as:

- Permanent world bots with persistent characters, equipment, progression, and state
- Bot questing, grouping, combat, training, vendors, travel, and other world interactions
- Complex bot and player chat powered by external language-model services
- Persistent bot memories, relationships, goals, and behavioral context
- Web-based account and character management
- Live world telemetry and map visualization
- Remote administration and server control
- Bot party and raid coordination
- Web-based world and spawn editing
- Custom gameplay systems exposed through MangosSuperUI
- Server-to-web communication and real-time status updates
- Tools for inspecting, debugging, and validating world content

## Custom gameplay systems

SuperUI-Core also includes systems that extend how quests, crafting, loot, and world content can behave.

Examples include:

- **Lootifier** systems for generating or selecting item variants based on configurable rarity, level, and progression rules
- **Questifier** systems for generating or selecting quest rewards from configurable item pools
- Crafting reward variation
- Expanded bot-aware quest, loot, vendor, and group handling
- Additional commands and APIs used by MangosSuperUI
- Configurable custom behavior that can be enabled or disabled by the server administrator

These systems are under active development and may change as MangosSuperUI and SuperUI-Core evolve.

## Progressive Vanilla foundation

Because SuperUI-Core is based on VMaNGOS, it retains support for progressive Vanilla content and patch-appropriate game clients.

### Currently supported client builds

- 1.12.1.5875+
- 1.11.2.5464
- 1.10.2.5302
- 1.9.4.5086
- 1.8.4.4878
- 1.7.1.4695
- 1.6.1.4544
- 1.5.1.4449

<!--
- 1.4.2.4375
- 1.3.1.4297
- 1.2.4.4222
-->

## Project goals

### Preserve the VMaNGOS foundation

SuperUI-Core is not intended to obscure or replace its origin. It is a VMaNGOS fork and depends heavily on the work of the VMaNGOS, Elysium, Light's Hope, MaNGOS, and broader emulator communities.

### Build a persistent simulated world

Bots should exist as persistent inhabitants rather than temporary scripted helpers. Their characters, equipment, progression, relationships, actions, and memories should continue across sessions.

### Make complex systems manageable

MangosSuperUI is intended to expose server systems through understandable interfaces rather than requiring every action to be performed through SQL, configuration files, console commands, or direct C++ development.

### Improve debugging and content validation

The project aims to provide workflows for identifying broken spawns, missing assets, pathing problems, quest issues, map errors, and other content failures, then reviewing and resolving them efficiently.

### Keep custom systems configurable

Custom gameplay behavior should generally be controlled through configuration options so server administrators can choose which SuperUI systems they want to use.

## Downloads

- [![Linux Development Build](https://github.com/Yafrovon/SuperUI-Core/actions/workflows/linux-development-release.yaml/badge.svg?branch=main)](https://github.com/Yafrovon/SuperUI-Core/actions/workflows/linux-development-release.yaml) Current development build status
- [Development database and installation information](https://github.com/vmangos/core)

Precompiled releases may not always be available while the project is under active development.

## Related projects

- [MangosSuperUI](https://github.com/Yafrovon/MangosSuperUI)
- [VMaNGOS Core](https://github.com/vmangos/core)
- [VMaNGOS Wiki](https://github.com/vmangos/wiki)
- [VMaNGOS Discord](https://discord.gg/x9a2jt7)
- [VMaNGOS Script Editor](https://github.com/brotalnia/scripteditor)
- [VMaNGOS Script Converter](https://github.com/vmangos/ScriptConverter)

## Project status

SuperUI-Core is under active development.

The project currently serves as the core used by the MangosSuperUI development environment and private server implementation. Some systems may be experimental, incomplete, or tailored to the current MangosSuperUI architecture.

It should not yet be assumed to function as a drop-in replacement for every existing VMaNGOS installation.

## Credits

SuperUI-Core exists because of the extensive work completed by the VMaNGOS contributors and the projects that preceded it, including MaNGOS, Elysium, and Light's Hope.

The custom SuperUI systems are built on top of that foundation and should not be interpreted as an independent recreation of the underlying emulator.

## License

SuperUI-Core retains the licensing requirements of the upstream VMaNGOS project.

See the repository's [GPL-2.0 license](LICENSE) for details.
