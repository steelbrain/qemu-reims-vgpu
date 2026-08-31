hw_display_modules = {}

system_ss.add(when: 'CONFIG_DDC', if_true: files('i2c-ddc.c'))
system_ss.add(when: 'CONFIG_EDID', if_true: files('edid-generate.c', 'edid-region.c'))

system_ss.add(when: 'CONFIG_FW_CFG_DMA', if_true: files('ramfb.c'))
stub_ss.add(files('ramfb-stubs.c'))
system_ss.add(when: 'CONFIG_FW_CFG_DMA', if_true: files('ramfb-standalone.c'))

system_ss.add(when: 'CONFIG_VGA_CIRRUS', if_true: files('cirrus_vga.c'))
system_ss.add(when: ['CONFIG_VGA_CIRRUS', 'CONFIG_VGA_ISA'], if_true: files('cirrus_vga_isa.c'))
system_ss.add(when: 'CONFIG_G364FB', if_true: files('g364fb.c'))
system_ss.add(when: 'CONFIG_JAZZ_LED', if_true: files('jazz_led.c'))
system_ss.add(when: 'CONFIG_PL110', if_true: files('pl110.c'))
system_ss.add(when: 'CONFIG_IMX6UL_LCDIF', if_true: files('imx6ul_lcdif.c'))
system_ss.add(when: 'CONFIG_SII9022', if_true: files('sii9022.c'))
system_ss.add(when: 'CONFIG_SSD0303', if_true: files('ssd0303.c'))
system_ss.add(when: 'CONFIG_SSD0323', if_true: files('ssd0323.c'))
system_ss.add(when: 'CONFIG_XEN_BUS', if_true: files('xenfb.c'))

system_ss.add(when: 'CONFIG_VGA_PCI', if_true: files('vga-pci.c'))
system_ss.add(when: 'CONFIG_VGA_ISA', if_true: files('vga-isa.c'))
system_ss.add(when: 'CONFIG_VGA_MMIO', if_true: files('vga-mmio.c'))
system_ss.add(when: 'CONFIG_VMWARE_VGA', if_true: files('vmware_vga.c'))
system_ss.add(when: 'CONFIG_BOCHS_DISPLAY', if_true: files('bochs-display.c'))

system_ss.add(when: 'CONFIG_EXYNOS4', if_true: files('exynos4210_fimd.c'))
system_ss.add(when: 'CONFIG_FRAMEBUFFER', if_true: files('framebuffer.c'))

system_ss.add(when: 'CONFIG_RASPI', if_true: files('bcm2835_fb.c'))
system_ss.add(when: 'CONFIG_SM501', if_true: files('sm501.c'))
system_ss.add(when: 'CONFIG_TCX', if_true: files('tcx.c'))
system_ss.add(when: 'CONFIG_CG3', if_true: files('cg3.c'))
system_ss.add(when: 'CONFIG_MACFB', if_true: files('macfb.c'))
system_ss.add(when: 'CONFIG_NEXTCUBE', if_true: files('next-fb.c'))

system_ss.add(when: 'CONFIG_VGA', if_true: files('vga.c'))
system_ss.add(when: 'CONFIG_VIRTIO', if_true: files('virtio-dmabuf.c'))
system_ss.add(when: 'CONFIG_DM163', if_true: files('dm163.c'))

stub_ss.add([files('acpi-vga-stub.c'), pixman])
if (config_all_devices.has_key('CONFIG_VGA_CIRRUS') or
    config_all_devices.has_key('CONFIG_VGA_PCI') or
    config_all_devices.has_key('CONFIG_VMWARE_VGA') or
    config_all_devices.has_key('CONFIG_ATI_VGA')
   )
  system_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.c'))
endif

if config_all_devices.has_key('CONFIG_QXL')
  qxl_ss = ss.source_set()
  qxl_ss.add(when: 'CONFIG_QXL', if_true: [files('qxl.c', 'qxl-logger.c', 'qxl-render.c'),
                                           pixman, spice])
  qxl_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.c'))
  hw_display_modules += {'qxl': qxl_ss}
endif

system_ss.add(when: 'CONFIG_DPCD', if_true: files('dpcd.c'))
system_ss.add(when: 'CONFIG_XLNX_DISPLAYPORT', if_true: files('xlnx_dp.c'))

system_ss.add(when: 'CONFIG_ARTIST', if_true: files('artist.c'))

system_ss.add(when: 'CONFIG_ATI_VGA', if_true: [files('ati.c', 'ati_2d.c', 'ati_dbg.c'), pixman])

system_ss.add(when: [pvg, 'CONFIG_MAC_PVG_PCI'],     if_true: [files('apple-gfx.m', 'apple-gfx-pci.m')])
system_ss.add(when: [pvg, 'CONFIG_MAC_PVG_MMIO'],    if_true: [files('apple-gfx.m', 'apple-gfx-mmio.m')])

if config_all_devices.has_key('CONFIG_REIMS_VGPU')
  # Thin QEMU C device links crates/reims-vgpu (Cargo staticlib). Which backends
  # are *linked in* is a configure-time choice baked into the binary, never a
  # runtime env sniff.
  #
  # `both` links both and lets the device pick between them at run time
  # (REIMS_VGPU_RAIL). That is not a relaxation of the rule above — it is what
  # makes the rule affordable. An Apple host reaches its GPU natively through
  # Metal and through MoltenVK, and telling a metal2vulkan defect from a defect
  # in this device means running the *same guest stream* both ways. With one
  # backend per binary that comparison costs a rebuild, which changes the
  # binary, the caches and the boot alongside the thing being measured.
  # Sources live in the parent repo, two directories above this vendored submodule.
  reims_vgpu_repo_dir = meson.global_source_root() / '..' / '..'
  reims_vgpu_abi_header_dir = reims_vgpu_repo_dir / 'crates' / 'reims-vgpu' / 'include'
  # A meson option and not the environment, because meson re-runs this file
  # whenever ninja decides the build is out of date — and that regeneration
  # inherits ninja's environment, not the one the configure ran under. A build
  # configured for Vulkan therefore silently became a Metal one the first time
  # any meson.build changed under a plain `ninja`, and on Linux that build fails
  # with twenty Rust errors from an Apple-only arm. An option is stored in the
  # build directory and survives regeneration, which is the property this needs.
  #
  # `env` keeps `REIMS_VGPU_BACKEND` working for a caller that has not been
  # taught the option yet, and it is only ever read on a real configure —
  # `scripts/qemu-build` passes the option, so the fallback is not on the path
  # any boot takes.
  reims_vgpu_backend_kind = get_option('reims_vgpu_backend')
  if reims_vgpu_backend_kind == 'env'
    reims_vgpu_backend_kind = run_command(find_program('sh'), '-c',
      'printf %s "${REIMS_VGPU_BACKEND:-metal}"', check: true).stdout().strip()
  endif
  cargo = find_program('cargo')
  # Rust staticlib always needs these on Unix; Apple frameworks only on Darwin.
  reims_vgpu_rust_sys_libs = ['-lm', '-ldl', '-lpthread']
  if host_os == 'darwin'
    reims_vgpu_rust_sys_libs += ['-liconv']
  elif host_os == 'windows'
    # mingw-w64: no libm/libdl to link; winit's native windowing stack and the
    # sockets lib the Rust std target expects.
    reims_vgpu_rust_sys_libs = ['-luser32', '-lgdi32', '-ldwmapi', '-lole32',
                                '-lshell32', '-limm32', '-loleaut32', '-lws2_32',
                                '-luserenv', '-lntdll', '-luxtheme']
  endif
  if reims_vgpu_backend_kind == 'metal'
    # host-window: the Metal rail presents through a Rust-owned winit window and
    # a CAMetalLayer on its view, the same window the Vulkan rail fills through a
    # swapchain. Which rail fills it is a run-time answer, so this feature is on
    # for every arm that ships one; QEMU's own display is then unused and the
    # boot passes -display none.
    reims_vgpu_cargo_features = '--features backend-metal,host-window'
    if host_os == 'darwin'
      # The same list as the vulkan and both arms below: winit's native
      # windowing stack is what widened it past Metal + QuartzCore, and all
      # three arms now carry a window. Spelled out in full rather than shared,
      # for the reason the 'both' arm gives.
      reims_vgpu_link_args = [
        '-framework', 'AppKit',
        '-framework', 'ApplicationServices',
        '-framework', 'Carbon',
        '-framework', 'CoreFoundation',
        '-framework', 'CoreGraphics',
        '-framework', 'CoreVideo',
        '-framework', 'Foundation',
        '-framework', 'Metal',
        '-framework', 'QuartzCore',
        '-lobjc',
      ] + reims_vgpu_rust_sys_libs
    else
      # Host metal stubs: no Apple frameworks.
      reims_vgpu_link_args = reims_vgpu_rust_sys_libs
    endif
  elif reims_vgpu_backend_kind == 'vulkan'
    # host-window adds the Rust-owned winit + VkSurfaceKHR presentation window
    # (kb host-window), opt-in at runtime behind REIMS_VGPU_WINDOW. Always built into
    # the vulkan binary; reims_vgpu_qemu_window_start returns ERR_STATE if unused.
    reims_vgpu_cargo_features = '--no-default-features --features backend-vulkan,host-window'
    # ash loads the Vulkan loader at runtime; no link-time Vulkan dep.
    if host_os == 'darwin'
      # winit/raw-window-metal link against the native macOS windowing stack even
      # when REIMS_VGPU_WINDOW is off at runtime. Keep the Rust staticlib dependency
      # self-contained so a Darwin Vulkan QEMU link does not depend on Cocoa
      # already being enabled elsewhere in QEMU.
      reims_vgpu_link_args = [
        '-framework', 'AppKit',
        '-framework', 'ApplicationServices',
        '-framework', 'Carbon',
        '-framework', 'CoreFoundation',
        '-framework', 'CoreGraphics',
        '-framework', 'CoreVideo',
        '-framework', 'Foundation',
        '-framework', 'Metal',
        '-framework', 'QuartzCore',
        '-lobjc',
      ] + reims_vgpu_rust_sys_libs
    else
      # Linux window backends are loaded by winit/ash-window at runtime.
      reims_vgpu_link_args = reims_vgpu_rust_sys_libs
      # Runtime-dead measurement (scripts/runtime-dead). When the staticlib is
      # built with -C instrument-coverage, its objects carry __llvm_prf_* but
      # nothing that writes them: rustc bundles the profiler runtime into a
      # final artifact, and this staticlib is not one — it is linked by QEMU.
      # So the profile runtime is named here instead, whole-archive because no
      # instrumented object references it and the linker would drop every
      # member. REIMS_VGPU_COVERAGE holds the path to
      # libclang_rt.profile-<arch>.a; unset (the default) changes nothing.
      reims_vgpu_coverage_rt = run_command('sh', '-c',
        'printf %s "$REIMS_VGPU_COVERAGE"', check: false).stdout().strip()
      if reims_vgpu_coverage_rt != ''
        reims_vgpu_link_args += ['-Wl,--whole-archive', reims_vgpu_coverage_rt,
                                 '-Wl,--no-whole-archive']
      endif
    endif
  elif reims_vgpu_backend_kind == 'both'
    # Metal + Vulkan in one staticlib, chosen at run time. Apple only: the crate
    # rejects backend-metal off macOS, because there is no host-stub Metal arm.
    if host_os != 'darwin'
      error('reims_vgpu_backend=both requires an Apple host (backend-metal has ' +
            'no host-stub arm); use metal or vulkan')
    endif
    reims_vgpu_cargo_features = '--features backend-metal,backend-vulkan,host-window'
    # The union of the two arms' frameworks, which on Darwin is the Vulkan list:
    # it already names Metal, QuartzCore, Foundation and CoreGraphics for winit
    # and raw-window-metal, so the Metal arm adds nothing to it. Spelled out in
    # full rather than derived from the branch above, because a reader checking
    # what this binary links should not have to evaluate two branches to find
    # out, and because the day the two lists stop overlapping this must not
    # silently drop one of them.
    reims_vgpu_link_args = [
      '-framework', 'AppKit',
      '-framework', 'ApplicationServices',
      '-framework', 'Carbon',
      '-framework', 'CoreFoundation',
      '-framework', 'CoreGraphics',
      '-framework', 'CoreVideo',
      '-framework', 'Foundation',
      '-framework', 'Metal',
      '-framework', 'QuartzCore',
      '-lobjc',
    ] + reims_vgpu_rust_sys_libs
  else
    error('unknown REIMS_VGPU_BACKEND: ' + reims_vgpu_backend_kind +
          ' (metal | vulkan | both)')
  endif
  reims_vgpu_built = reims_vgpu_repo_dir / 'target' / 'release' / 'libreims_vgpu.a'
  reims_vgpu_a = custom_target('reims-vgpu-staticlib',
    output: 'libreims_vgpu.a',
    console: true,
    build_always_stale: true,
    command: [find_program('sh'), '-c',
      '"' + cargo.full_path() + '" build --release --manifest-path "' +
      reims_vgpu_repo_dir + '/Cargo.toml" -p reims-vgpu ' + reims_vgpu_cargo_features +
      ' && ( cmp -s "' + reims_vgpu_built + '" "@OUTPUT@" || cp "' +
      reims_vgpu_built + '" "@OUTPUT@" )'])
  # Header lives outside the qemu submodule tree; use compile_args -I.
  reims_vgpu_dep = declare_dependency(
    link_with: reims_vgpu_a,
    link_args: reims_vgpu_link_args,
    compile_args: ['-I' + reims_vgpu_abi_header_dir])
  system_ss.add(when: 'CONFIG_REIMS_VGPU',
    if_true: [files('reims-vgpu-mmio.c', 'reims-vgpu-pci.c', 'reims-vgpu-dirty.c',
                    'reims-vgpu-shim.c'), reims_vgpu_dep])
endif

if config_all_devices.has_key('CONFIG_VIRTIO_GPU')
  virtio_gpu_ss = ss.source_set()
  virtio_gpu_ss.add(when: 'CONFIG_VIRTIO_GPU',
                    if_true: [files('virtio-gpu-base.c', 'virtio-gpu.c'), pixman])
  if host_os == 'linux'
    virtio_gpu_ss.add(files('virtio-gpu-udmabuf.c'))
  else
    virtio_gpu_ss.add(files('virtio-gpu-udmabuf-stubs.c'))
  endif
  virtio_gpu_ss.add(when: 'CONFIG_VHOST_USER_GPU', if_true: files('vhost-user-gpu.c'))
  hw_display_modules += {'virtio-gpu': virtio_gpu_ss}

  if virgl.found() and opengl.found()
    virtio_gpu_gl_ss = ss.source_set()
    virtio_gpu_gl_ss.add(when: ['CONFIG_VIRTIO_GPU', virgl, opengl],
                         if_true: [files('virtio-gpu-gl.c', 'virtio-gpu-virgl.c'), pixman, virgl])
    hw_display_modules += {'virtio-gpu-gl': virtio_gpu_gl_ss}
  endif

  if rutabaga.found()
    virtio_gpu_rutabaga_ss = ss.source_set()
    virtio_gpu_rutabaga_ss.add(when: ['CONFIG_VIRTIO_GPU', rutabaga],
                               if_true: [files('virtio-gpu-rutabaga.c'), pixman])
    hw_display_modules += {'virtio-gpu-rutabaga': virtio_gpu_rutabaga_ss}
  endif
endif

if config_all_devices.has_key('CONFIG_VIRTIO_PCI')
  virtio_gpu_pci_ss = ss.source_set()
  virtio_gpu_pci_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI'],
                        if_true: [files('virtio-gpu-pci.c'), pixman])
  virtio_gpu_pci_ss.add(when: ['CONFIG_VHOST_USER_GPU', 'CONFIG_VIRTIO_PCI'],
                        if_true: files('vhost-user-gpu-pci.c'))
  hw_display_modules += {'virtio-gpu-pci': virtio_gpu_pci_ss}

  if virgl.found() and opengl.found()
    virtio_gpu_pci_gl_ss = ss.source_set()
    virtio_gpu_pci_gl_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI', virgl, opengl],
                             if_true: [files('virtio-gpu-pci-gl.c'), pixman])
    hw_display_modules += {'virtio-gpu-pci-gl': virtio_gpu_pci_gl_ss}
  endif
  if rutabaga.found()
    virtio_gpu_pci_rutabaga_ss = ss.source_set()
    virtio_gpu_pci_rutabaga_ss.add(when: ['CONFIG_VIRTIO_GPU', 'CONFIG_VIRTIO_PCI', rutabaga],
                                   if_true: [files('virtio-gpu-pci-rutabaga.c'), pixman])
    hw_display_modules += {'virtio-gpu-pci-rutabaga': virtio_gpu_pci_rutabaga_ss}
  endif
endif

if config_all_devices.has_key('CONFIG_VIRTIO_VGA')
  virtio_vga_ss = ss.source_set()
  virtio_vga_ss.add(when: 'CONFIG_VIRTIO_VGA',
                    if_true: [files('virtio-vga.c'), pixman])
  virtio_vga_ss.add(when: 'CONFIG_VHOST_USER_VGA',
                    if_true: files('vhost-user-vga.c'))
  virtio_vga_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.c'))
  hw_display_modules += {'virtio-vga': virtio_vga_ss}

  if virgl.found() and opengl.found()
    virtio_vga_gl_ss = ss.source_set()
    virtio_vga_gl_ss.add(when: ['CONFIG_VIRTIO_VGA', virgl, opengl],
                         if_true: [files('virtio-vga-gl.c'), pixman])
    virtio_vga_gl_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.c'))
    hw_display_modules += {'virtio-vga-gl': virtio_vga_gl_ss}
  endif

  if rutabaga.found()
    virtio_vga_rutabaga_ss = ss.source_set()
    virtio_vga_rutabaga_ss.add(when: ['CONFIG_VIRTIO_VGA', rutabaga],
                               if_true: [files('virtio-vga-rutabaga.c'), pixman])
    virtio_vga_rutabaga_ss.add(when: 'CONFIG_ACPI', if_true: files('acpi-vga.c'))
    hw_display_modules += {'virtio-vga-rutabaga': virtio_vga_rutabaga_ss}
  endif
endif

system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_lcdc.c'))

modules += { 'hw-display': hw_display_modules }
