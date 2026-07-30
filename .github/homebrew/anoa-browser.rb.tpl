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

  binary "#{appdir}/anoa-browser.app/Contents/MacOS/anoa-browser"
  binary "anoa-term"

  # The app is ad-hoc signed, not notarized. Homebrew tags every downloaded
  # cask with com.apple.quarantine; if the app is ever launched while that flag
  # is set, Gatekeeper caches a rejection (by cdhash) and the kernel then
  # SIGKILLs it forever — even after the flag is removed. So the flag MUST be
  # cleared here, before the user's first launch. `xattr -dr` does not exist on
  # macOS 26, so recurse with find; `|| true` keeps a missing attr from failing
  # the install.
  postflight do
    # Target the installed location (appdir) — by the time postflight runs the
    # .app has already been moved out of the Caskroom staging dir.
    system_command "/bin/sh",
                   args: ["-c",
                          "/usr/bin/find #{appdir.join("anoa-browser.app").to_s.shellescape} " \
                          "-exec /usr/bin/xattr -d com.apple.quarantine {} + 2>/dev/null || true"]
  end

  caveats <<~EOS
    anoa-browser is ad-hoc signed, not notarized. This cask clears the
    quarantine flag on install so it launches normally. If macOS still blocks
    it (e.g. after a manual move), clear the flag yourself:
      find "#{appdir}/anoa-browser.app" -exec xattr -d com.apple.quarantine {} +
  EOS

  zap trash: [
    "~/Library/Caches/anoa-browser",
    "~/Library/Preferences/com.porcupine-md.anoa-browser.plist",
  ]
end
