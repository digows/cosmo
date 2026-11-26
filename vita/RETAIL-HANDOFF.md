# Vita retail testing handoff

## State at handoff

- The retail Vita runs firmware **3.65 with HENkaku Ensō**. The owner confirmed that VitaShell and existing applications work.
- The baseline is **Enhanced Cosmo Episode 1 with SDL3/GXM and MP3 remixes**, from commit [`34f9c7d`](https://github.com/GITitdoneboi/enhanced-cosmo/tree/34f9c7d384df5131cc6cf4d080119049fd11ee83). Hold AAC and gameplay timing work until the retail baseline has been captured and reviewed. The earlier AAC experiments were local and outside this handoff.
- The icon-corrected MP3 package was built with VitaSDK **2026.08.1**, CMake **3.31.8**, Ninja **1.13.2**, and SDL **3.4.12**. Its local filename was `cosmo-vita-34f9c7d-iconfix.vpk`. SHA-256 `88BBAED4357BAF8AFD10272AA19FE8C5AA0DAFD208B9695DE4D9D4F18876D790`. The hash identifies that specific local artifact. Rebuild instructions are below.
- The bridge checkout was [`9f2ef15`](https://github.com/shoui520/vita-agent-use/tree/9f2ef155d05a3b488fdf6e140696ab2bbbf84f7c). Mbed TLS **3.6.7**, including its pinned submodule, was fetched. VitaSDK's `taihen` and `zlib` packages were installed.
- Windows CMake generated Ninja files for the bridge. Ninja then stopped because `stubs/libvita_agent_kernel_stub.a` had no generating rule at that path. At handoff, the three modules still need a verified build. Native Windows Python 3.13 with `cryptography` and `windows-curses` installed still failed at the client's `fcntl` import. Use a POSIX Python environment for the upstream client's file locks, private identity files and server socket.
- The owner supplied the Vita's local address in chat. Confirm the address from VitaShell on the new network. The taiHEN backup was requested and is awaiting confirmation. Plugin installation, pairing and all retail measurements remain pending. Keep local addresses, taiHEN backups, certificates, tokens and raw logs in private workstation storage.

## Build the bridge on the new workstation

Use a Linux shell on the same local network as the Vita. On Windows, WSL2 is the VitaSDK-recommended environment. Install VitaSDK in that environment and set `VITASDK` and `PATH`. The native Windows SDK and a Linux SDK use different host binaries. Install Git, CMake, Ninja, Make, Python 3 and Python's venv support.

```sh
vdpm install taihen zlib
git clone https://github.com/shoui520/vita-agent-use.git
git -C vita-agent-use checkout 9f2ef155d05a3b488fdf6e140696ab2bbbf84f7c
git clone --branch mbedtls-3.6.7 --recurse-submodules \
  https://github.com/Mbed-TLS/mbedtls.git mbedtls-3.6.7

cmake -S vita-agent-use -B vita-agent-build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DVAU_MBEDTLS_SOURCE="$PWD/mbedtls-3.6.7"
cmake --build vita-agent-build --target vau_stubs
cmake --build vita-agent-build

python3 -m venv vita-agent-venv
. vita-agent-venv/bin/activate
python -m pip install cryptography
python vita-agent-use/client/vita_agent.py --help
```

Building `vau_stubs` first is the next workaround to try for the Windows Ninja dependency failure. Verify that it creates the stub archive and that the full build produces all three modules. Record the bridge revision and SHA-256 of each module before transfer. [VitaSDK setup](https://vitasdk.org/) · [Bridge build instructions](https://github.com/shoui520/vita-agent-use#installation) · [Mbed TLS 3.6.7](https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7)

## Install, back up and pair

1. In VitaShell, identify the active taiHEN `config.txt` and copy it to a separate file, such as `ux0:data/tai-config-before-vita-agent.txt`. Keep the original intact until the backup is confirmed. A second copy on the workstation is useful for recovery.
2. Copy `vita_agent_kernel.skprx`, `vita_agent_loader.suprx`, and `vita_agent_shell.suprx` to `ur0:tai/`. Add only the following entries under the existing sections of the active config. The loader starts the Shell module.
3. Review the resulting config, then reboot the Vita. Make sure VitaShell and LiveArea still work. Start VitaShell FTP for the PC bootstrap.
4. Run `python vita-agent-use/client/vita_agent.py bootstrap` interactively. Enter the Vita's current LAN address and choose a private config directory. Bootstrap checks the plugins over VitaShell FTP. Use the `VITA_AGENT_CONFIG_DIR` it prints.
5. Run `python vita-agent-use/client/vita_agent.py session pair` and approve the prompt **on the Vita**. A new workstation can use its own identity and pairing. Run `serve` in a separate long-lived terminal, confirm `server status` is connected, and monitor `server events` for crash, disconnect and log events. Turn off VitaShell FTP after setup. Keep the bridge on the local network.

```text
*KERNEL
ur0:tai/vita_agent_kernel.skprx

*main
ur0:tai/vita_agent_loader.suprx
```

[Vita installation and pairing](https://github.com/shoui520/vita-agent-use#installation) · [CLI and event reference](https://github.com/shoui520/vita-agent-use/tree/main/docs/agent-reference)

## Rebuild and verify the MP3 baseline

1. Check out `34f9c7d384df5131cc6cf4d080119049fd11ee83` in a clean checkout and run `git submodule update --init --recursive`. Use the [Vita build instructions](README.md) and the [Episode 1 data checksums](../gamedata/README.md).
2. Correct the source LiveArea icon before packaging. It must be an opaque **128×128, 8-bit indexed PNG** with PNG color type 3. The original baseline source and first VPK used RGBA. The following ImageMagick command reproduces the format conversion used for the corrected artifact.

   ```sh
   magick vita/sce_sys/icon0.png -background black -alpha remove -alpha off \
     +dither -colors 256 -depth 8 -strip -define png:color-type=3 \
     -define png:bit-depth=8 vita/sce_sys/icon0-indexed.png
   mv vita/sce_sys/icon0-indexed.png vita/sce_sys/icon0.png
   ```

3. Build the VPK and inspect the ZIP entries for `eboot.bin`, `sce_sys/param.sfo`, the corrected embedded icon, `COSMO1.STN`, `COSMO1.VOL`, and the MP3 music files. Keep the package hash with the run. Check whether title `CSMO00001` is installed and confirm before replacing a different Cosmo build. [Vita icon requirements](https://www.psdevwiki.com/vita/index.php?title=Icon0.png)
4. Verify the bridge with a system snapshot, a screenshot and reversible remote input. Upload the VPK with checksum verification, install it through the bridge, then confirm deployment and launch from the returned status. Select **Forbidden Planet**, verify the remixed soundtrack is enabled, and test movement, jumping and a level transition.
5. Capture menu and gameplay stills and a separate motion sequence. The bridge reports the actual screenshot format. Use native PNG or raw captures when available and assemble motion as GIF. Capture media in a separate pass from performance measurement so capture I/O does not skew baseline timing. [Bridge commands](https://github.com/shoui520/vita-agent-use/blob/main/docs/agent-reference/commands.md)

## Retail measurements and review

- Record console model, firmware, CPU/GPU clocks, active plugins, VPK hash, gameplay route, audio settings and capture settings. Use the same conditions for later AAC and timing comparisons.
- Collect repeated bridge performance windows for framebuffer submission FPS and foreground USER_RW and CDRAM memory. Record actual sample windows, peak observed RAM/VMEM and any dropped samples. Aggregate FPS is a separate metric from per-frame pacing. Use presentation timestamps to report frame-time percentiles and missed frames.
- Measure audio callback wall time and invocation count, including worst callback and total audio-thread time per displayed frame. The target is **under 1 ms per frame**. Use a profiling-only MP3 baseline if callbacks need instrumentation. The bridge's system-wide per-core CPU metric is insufficient for callback timing. Use [libperf](https://github.com/shoui520/libperf) when thread CPU cycles or cache counters are needed. Upstream libperf revision fetched was `3905aa16965904528199da7aecd9a4f32b18d224`. Review and back up the config before any additional kernel plugin is installed.
- Keep original capture and timing data privately. Review the baseline with the project owner before selecting the AAC or timing follow-up. [Bridge metrics and event semantics](https://github.com/shoui520/vita-agent-use/blob/main/docs/agent-reference/events-runs.md#performance)
