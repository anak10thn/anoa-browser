cask "anoa-browser" do
  version "{{VERSION}}"
  sha256 "{{MACOS_SHA256}}"

  # Universal (x86_64 + arm64) build — one archive for Intel and Apple Silicon.
  url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-browser-macos-universal.tar.gz"
  name "Anoa Browser"
  desc "Headless browser built on Qt6/QWebEngine with CDP support"
  homepage "https://github.com/porcupine-md/anoa-browser"

  depends_on macos: :monterey

  app "anoa-browser.app"

  # Both binaries live inside the bundle so they are covered by the app's
  # signature and notarization ticket.
  binary "#{appdir}/anoa-browser.app/Contents/MacOS/anoa-browser"
  binary "#{appdir}/anoa-browser.app/Contents/MacOS/anoa-term"

  # Safety net: strip com.apple.quarantine before the app is ever launched.
  # For a properly notarized+stapled release this is a harmless no-op (Gatekeeper
  # trusts the stapled ticket). It only matters for an ad-hoc fallback build,
  # where launching while quarantined would make Gatekeeper cache a reject by
  # cdhash and the kernel SIGKILL the app thereafter. `xattr -dr` does not exist
  # on macOS 26, so recurse with find; `|| true` tolerates a missing attribute.
  postflight do
    system_command "/bin/sh",
                   args: ["-c",
                          "/usr/bin/find #{appdir.join("anoa-browser.app").to_s.shellescape} " \
                          "-exec /usr/bin/xattr -d com.apple.quarantine {} + 2>/dev/null || true"]
  end

  zap trash: [
    "~/Library/Caches/anoa-browser",
    "~/Library/Preferences/com.porcupine-md.anoa-browser.plist",
  ]
end
