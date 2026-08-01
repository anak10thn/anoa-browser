cask "anoa" do
  version "{{VERSION}}"
  sha256 "{{MACOS_SHA256}}"

  # Universal (x86_64 + arm64) build — one archive for Intel and Apple Silicon.
  url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-macos-universal.tar.gz"
  name "Anoa Browser"
  desc "Headless browser built on Qt6/QWebEngine with CDP support"
  homepage "https://github.com/porcupine-md/anoa-browser"

  depends_on macos: :monterey

  app "anoa.app"

  # The binary lives inside the bundle so it is covered by the app's signature
  # and notarization ticket. The terminal viewer is a subcommand of it
  # (`anoa terminal`), so one shim covers both entry points.
  binary "#{appdir}/anoa.app/Contents/MacOS/anoa"

  # Safety net: strip com.apple.quarantine before the app is ever launched.
  # For a properly notarized+stapled release this is a harmless no-op (Gatekeeper
  # trusts the stapled ticket). It only matters for an ad-hoc fallback build,
  # where launching while quarantined would make Gatekeeper cache a reject by
  # cdhash and the kernel SIGKILL the app thereafter. `xattr -dr` does not exist
  # on macOS 26, so recurse with find; `|| true` tolerates a missing attribute.
  postflight do
    system_command "/bin/sh",
                   args: ["-c",
                          "/usr/bin/find #{appdir.join("anoa.app").to_s.shellescape} " \
                          "-exec /usr/bin/xattr -d com.apple.quarantine {} + 2>/dev/null || true"]
  end

  zap trash: [
    "~/Library/Caches/anoa",
    "~/Library/Preferences/com.porcupine-md.anoa.plist",
  ]
end
