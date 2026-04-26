cask "anoa-browser" do
  version "{{VERSION}}"
  sha256 "{{MACOS_SHA256}}"

  url "https://github.com/porcupine-md/anoa-browser/releases/download/v#{version}/anoa-browser-macos.tar.gz"
  name "Anoa Browser"
  desc "Headless browser built on Qt6/QWebEngine with CDP support"
  homepage "https://github.com/porcupine-md/anoa-browser"

  depends_on macos: ">= :monterey"

  app "anoa-browser.app"

  binary "#{appdir}/anoa-browser.app/Contents/MacOS/anoa-browser"

  zap trash: [
    "~/Library/Caches/anoa-browser",
    "~/Library/Preferences/com.porcupine-md.anoa-browser.plist",
  ]
end